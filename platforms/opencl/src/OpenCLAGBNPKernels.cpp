/* -------------------------------------------------------------------------- *
 *                                   OpenMM-AGBNP                            *
 * -------------------------------------------------------------------------- */

#include "AGBNPUtils.h"
#include "OpenCLAGBNPKernels.h"
#include "OpenCLAGBNPKernelSources.h"
#include "openmm/internal/ContextImpl.h"
#include "openmm/opencl/OpenCLNonbondedUtilities.h"
#include "openmm/opencl/OpenCLForceInfo.h"
#include <cmath>
#include <cfloat>

#include <fstream>
#include <iomanip>
#include <algorithm>

#include "openmm/reference/SimTKOpenMMRealType.h"
#include "openmm/reference/RealVec.h"
#include "gaussvol.h"

//conversion factors
//#define ANG (0.1f)
//#define ANG3 (0.001f)

//volume cutoffs in switching function
//#define MIN_GVOL (FLT_MIN)
#define VOLMIN0 (0.009f*ANG3)
//#define VOLMINA (0.01f*ANG3)
//#define VOLMINB (0.1f*ANG3)

#ifndef PI
#define PI (3.14159265359)
#endif

// conversion factors from spheres to Gaussians
//#define KFC (2.2269859253)

// minimum overlap volume to count
#define MAX_ORDER (12)

using namespace AGBNPPlugin;
using namespace OpenMM;
using namespace std;

class OpenCLAGBNPForceInfo : public OpenCLForceInfo {
public:
  OpenCLAGBNPForceInfo(int requiredBuffers, const AGBNPForce& force) : OpenCLForceInfo(requiredBuffers), force(force) {
    }
    int getNumParticleGroups() {
      return force.getNumParticles();//each particle is in a different group?
    }
    void getParticlesInGroup(int index, vector<int>& particles) {
      particles.push_back(index);
    }
    bool areGroupsIdentical(int group1, int group2) {
        return (group1 == group2);
    }
private:
    const AGBNPForce& force;
};

OpenCLCalcAGBNPForceKernel::~OpenCLCalcAGBNPForceKernel() {
  //    if (params != NULL)
  //   delete params;
}



static int _nov_ = 0;

void OpenCLCalcAGBNPForceKernel::init_tree_size(int pad_modulo,
					       vector<int>& noverlaps, vector<int>& noverlaps_2body)  {
  OpenCLNonbondedUtilities& nb = cl.getNonbondedUtilities();
  total_tree_size = 0;
  tree_size.clear();
  tree_pointer.clear();
  padded_tree_size.clear();
  atom_tree_pointer.clear();
  natoms_in_tree.clear();
  first_atom.clear();

  // compute number of heavy atoms
  int num_heavy = 0;
  for(int i = 0; i < cl.getNumAtoms(); i++){
    if(!atom_ishydrogen[i]) num_heavy += 1;
  }

  if(verbose_level > 0) std::cout << "Number of heavy atoms: " << num_heavy << std::endl; 

  //#define SMALL_SECTIONS
#ifdef SMALL_SECTIONS
  // this constructs many tree sections with 16 to 32 atoms per section
  int heavyatoms_per_section = num_heavy/num_compute_units;
  int nmin = OpenCLContext::TileSize/2;
  int nmax = OpenCLContext::TileSize;
  if(heavyatoms_per_section < nmin){
    // not enough atoms to feed the available compute units 
    // use fewer tree sections
    // place nmin heavy atoms in each section, and the remainder in the last.
    num_sections = num_heavy/nmin;
  }else if(heavyatoms_per_section > nmax){
    // more than minimum number of atoms per section
    // use a multiple of the number of compute devices to have at least
    // nmax atoms per section
    num_sections = num_compute_units;
    while(num_heavy/num_sections >= nmax){
      num_sections += num_compute_units;
    }
  }else{
    // within limits, use as many sections as compute units
    // distribute atoms equally among them
    num_sections = num_compute_units;
  }
  heavyatoms_per_section = num_heavy/num_sections;
  int extra_heavy_atoms = num_heavy % num_sections;
  natoms_in_tree.resize(num_sections);
  int iatom = 0;
  for(int section = 0 ; section < num_sections ; section++){
    natoms_in_tree[section] = 0;
    int nheavy_in_section = 0;
    int target_nheavy = heavyatoms_per_section;
    if(section < extra_heavy_atoms) target_nheavy += 1;
    while(nheavy_in_section < target_nheavy && iatom < cl.getNumAtoms()){
      if(!atom_ishydrogen[iatom]) nheavy_in_section += 1;
      iatom += 1;
      natoms_in_tree[section] += 1;
    }
  }
  //at this point all heavy atoms have been placed
  //trailing hydrogen atoms do not need to be inserted in tree

#else

  // this constructs as many tree sections as compute units
  // sections may contain many atoms

  num_sections = num_compute_units;
  int heavyatoms_per_section = num_heavy/num_sections;
  int extra_heavy_atoms = num_heavy % num_sections;
  int nmin = OpenCLContext::TileSize/2;
  if(heavyatoms_per_section >= nmin){
    // more than minimum number of atoms per section
    // standard behavior: heavy atoms are distributed among the available compute units
    // distribute extra heavy atoms to the top sections
    natoms_in_tree.resize(num_sections);
    int iatom = 0;
    for(int section = 0 ; section < num_sections ; section++){
      natoms_in_tree[section] = 0;
      int nheavy_in_section = 0;
      int target_nheavy = heavyatoms_per_section;
      if(section < extra_heavy_atoms) target_nheavy += 1;
      while(nheavy_in_section < target_nheavy && iatom < cl.getNumAtoms()){
	if(!atom_ishydrogen[iatom]) nheavy_in_section += 1;
	iatom += 1;
	natoms_in_tree[section] += 1;
      }
    }
    //at this point all heavy atoms have been placed
    //trailing hydrogen atoms do not need to be inserted in tree
  }else{
    // not enough atoms to feed the available compute units 
    // use fewer tree sections
    // place nmin heavy atoms in each section, and the remainder in the last.
    num_sections = num_heavy/nmin;
    natoms_in_tree.resize(num_sections);
    int iatom = 0;
    for(int section = 0 ; section < num_sections ; section++){
      natoms_in_tree[section] = 0;
      int nheavy_in_section = 0;
      int target_nheavy = nmin;
      while(nheavy_in_section < target_nheavy && iatom < cl.getNumAtoms()){
        if(!atom_ishydrogen[iatom]) nheavy_in_section += 1;
        iatom += 1;
        natoms_in_tree[section] += 1;
      }
    }
    //add last section if needed
    if(num_heavy % nmin != 0){
      num_sections += 1;
      natoms_in_tree.push_back(0);
      int section = num_sections - 1;
      int nheavy_in_section = 0;
      int target_nheavy = num_heavy % nmin;
      while(nheavy_in_section < target_nheavy &&  iatom < cl.getNumAtoms()){
	if(!atom_ishydrogen[iatom]) nheavy_in_section += 1;
        iatom += 1;
        natoms_in_tree[section] += 1;
      }
    }
  }
  
#endif


  total_atoms_in_tree = 0;
  for(int section = 0 ; section < num_sections; section++){
    total_atoms_in_tree += natoms_in_tree[section];
  }

  // computes size of each tree section
  int max_size = 0;
  int offset = 0;
  for(int section = 0; section < num_sections; section++){
    int nn = 0;
    for(int i = 0; i < natoms_in_tree[section] ; i++){
      int iat = offset + i;
      if(iat < cl.getNumAtoms()){
	nn += noverlaps[iat];
      }
    }
    if(nn>max_size) max_size = nn;
    offset += natoms_in_tree[section];
  }
  // double estimate
  int size = max_size * 2;
  // now pad
  int npadsize = pad_modulo*((size+pad_modulo-1)/pad_modulo);
  
  // set tree pointers
  tree_pointer.resize(num_sections);
  offset = 0;
  for(int section = 0; section < num_sections; section++){
    tree_pointer[section] = offset;
    offset += npadsize;
  }

  // set atom pointer in tree
  tree_size.resize(num_sections);
  padded_tree_size.resize(num_sections);
  atom_tree_pointer.resize(cl.getPaddedNumAtoms());
  first_atom.resize(num_sections);
  offset = 0;
  for(int section = 0; section < num_sections; section++){
    tree_size[section] = 0;
    padded_tree_size[section] = npadsize;
    first_atom[section] = offset;
    for(int i = 0; i <  natoms_in_tree[section]; i++){
      int iat = offset + i;
      int slot = section*npadsize + i;
      if(iat < total_atoms_in_tree){
	atom_tree_pointer[iat] = slot;
      }
    }
    total_tree_size += npadsize;
    offset += natoms_in_tree[section];
  }
}


int OpenCLCalcAGBNPForceKernel::copy_tree_to_device(void){

  int padsize = cl.getPaddedNumAtoms();
  vector<cl_int> nn(padsize);
  vector<cl_int> ns(num_sections);  

  for(int i = 0; i < padsize ; i++){
    nn[i] = (cl_int) atom_tree_pointer[i];
  }
  ovAtomTreePointer->upload(nn);

  for(int i = 0; i < num_sections ; i++){
    ns[i] = (cl_int) tree_pointer[i];
  }
  ovTreePointer->upload(ns);

  for(int i = 0; i < num_sections ; i++){
    ns[i] = (cl_int) tree_size[i];
  }
  ovAtomTreeSize->upload(ns);

  for(int i = 0; i < num_sections ; i++){
    ns[i] = (cl_int) padded_tree_size[i];
  }
  ovAtomTreePaddedSize->upload(ns);

  for(int i = 0; i < num_sections ; i++){
    ns[i] = (cl_int) natoms_in_tree[i];
  }
  ovNumAtomsInTree->upload(ns);

  for(int i = 0; i < num_sections ; i++){
    ns[i] = (cl_int) first_atom[i];
  }
  ovFirstAtom->upload(ns);

  return 1;
}

void OpenCLCalcAGBNPForceKernel::initialize(const System& system, const AGBNPForce& force) {
    verbose_level = 0;

    if (cl.getPlatformData().contexts.size() > 1)
      throw OpenMMException("AGBNPForce does not support using multiple OpenCL devices");
    
    OpenCLNonbondedUtilities& nb = cl.getNonbondedUtilities();
    int elementSize = (cl.getUseDoublePrecision() ? sizeof(cl_double) : sizeof(cl_float));   

    numParticles = cl.getNumAtoms();//force.getNumParticles();
    if (numParticles == 0)
        return;
    radiusParam1 = new OpenCLArray(cl, cl.getPaddedNumAtoms(), sizeof(cl_float), "radiusParam1");
    radiusParam2 = new OpenCLArray(cl, cl.getPaddedNumAtoms(), sizeof(cl_float), "radiusParam2");
    gammaParam1 = new OpenCLArray(cl, cl.getPaddedNumAtoms(), sizeof(cl_float), "gammaParam1");
    gammaParam2 = new OpenCLArray(cl, cl.getPaddedNumAtoms(), sizeof(cl_float), "gammaParam2");
    chargeParam = new OpenCLArray(cl, cl.getPaddedNumAtoms(), sizeof(cl_float), "chargeParam");
    alphaParam = new OpenCLArray(cl, cl.getPaddedNumAtoms(), sizeof(cl_float), "alphaParam");
    ishydrogenParam = new OpenCLArray(cl, cl.getPaddedNumAtoms(), sizeof(cl_int), "ishydrogenParam");

    testBuffer = new OpenCLArray(cl, cl.getPaddedNumAtoms(), sizeof(cl_float), "testBuffer");

    
    bool useLong = cl.getSupports64BitGlobalAtomics();

    // this the accumulation buffer for overlap atom-level data (self-volumes, etc.)
    // note that each thread gets a separate buffer of size Natoms (rather than each thread block as in the
    // non-bonded algorithm). This may limits the max number of atoms.

    //cl.addAutoclearBuffer(*ovAtomBuffer);

    vector<cl_float> radiusVector1(cl.getPaddedNumAtoms());
    vector<cl_float> radiusVector2(cl.getPaddedNumAtoms());
    vector<cl_float> gammaVector1(cl.getPaddedNumAtoms());
    vector<cl_float> gammaVector2(cl.getPaddedNumAtoms());
    vector<cl_float> chargeVector(cl.getPaddedNumAtoms());
    vector<cl_float> alphaVector(cl.getPaddedNumAtoms());
    vector<cl_int> ishydrogenVector(cl.getPaddedNumAtoms());
    atom_ishydrogen.resize(cl.getPaddedNumAtoms());

    for (int i = 0; i < numParticles; i++) {
      double radius, gamma, alpha, charge;
      bool ishydrogen;
      force.getParticleParameters(i, radius, gamma, alpha, charge, ishydrogen);
	radiusVector1[i] = (cl_float) radius;
	radiusVector2[i] = (cl_float) (radius - SA_DR);

	atom_ishydrogen[i] = ishydrogen;
	ishydrogenVector[i] = ishydrogen ? 1 : 0;

	// for surface-area energy use gamma/radius_offset
	// gamma = 1 for self volume calculation.
	double g = ishydrogen ? 0 : gamma/SA_DR;
	gammaVector1[i] = (cl_float)  g;
	gammaVector2[i] = (cl_float) -g;
	alphaVector[i] =  (cl_float) alpha;
	chargeVector[i] = (cl_float) charge;

    }
    radiusParam1->upload(radiusVector1);
    radiusParam2->upload(radiusVector2);
    gammaParam1->upload(gammaVector1);
    gammaParam2->upload(gammaVector2);
    alphaParam->upload(alphaVector);
    chargeParam->upload(chargeVector);
    ishydrogenParam->upload(ishydrogenVector);
    
    useCutoff = (force.getNonbondedMethod() != AGBNPForce::NoCutoff);
    usePeriodic = (force.getNonbondedMethod() != AGBNPForce::NoCutoff && force.getNonbondedMethod() != AGBNPForce::CutoffNonPeriodic);
    useExclusions = false;
    cutoffDistance = force.getCutoffDistance();
    if(verbose_level > 1){
      cout << "Cutoff distance: " << cutoffDistance << endl;
    }

    //initializes I4 lookup table for Born-radii calculation
    i4_rmin = 0.;
    i4_rmax = AGBNP_I4LOOKUP_MAXA;
    i4_table_size = AGBNP_I4LOOKUP_NA;
    vector<double> radii(numParticles);
    for(int i=0;i<numParticles;i++){
      radii[i] = (double)radiusVector1[i];
    }
    i4_lut = new AGBNPI42DLookupTable(radii, atom_ishydrogen, i4_table_size, i4_rmin, i4_rmax);

    //acquire and upload hash table of i4 tables
    AGBNPHtable *h_table = i4_lut->get_h_table();
    hsize = h_table->hsize;
    hmask = h_table->hmask;
    hjump = h_table->hjump;
    i4_hash_values = OpenCLArray::create<cl_int>(cl, hsize, "i4_hash_values");
    i4_hash_values->upload(h_table->values);

    //constructs and uploads tables to GPU
    int yy2size = 0;
    for(int ih = 0; ih < h_table->hsize ; ih++){
      int key = h_table->values[ih];
      if(key >= 0){
	AGBNPI4LookupTable *i4table = i4_lut->tables[ih];
	AGBNPLookupTable *lut_table = i4table->table;
	for(int i=0;i<i4_table_size;i++){
	  y_i4.push_back(lut_table->yt[i]);
	  y2_i4.push_back(lut_table->y2t[i]);
	  yy2size += 1;
	}
      }else{
	for(int i=0;i<i4_table_size;i++){
	  y_i4.push_back(0);
	  y2_i4.push_back(0);
	  yy2size += 1;
	}
      }
    } 

    i4YValues = OpenCLArray::create<cl_float>(cl, yy2size, "i4YValues");
    i4Y2Values = OpenCLArray::create<cl_float>(cl, yy2size, "i4Y2Values");
    i4YValues->upload(y_i4);
    i4Y2Values->upload(y2_i4);

    gvol_force = &force;
    niterations = 0;
    hasCreatedKernels = false;
}

double OpenCLCalcAGBNPForceKernel::execute(ContextImpl& context, bool includeForces, bool includeEnergy) {
  OpenCLNonbondedUtilities& nb = cl.getNonbondedUtilities();
  bool useLong = cl.getSupports64BitGlobalAtomics();
  bool verbose = verbose_level > 0;
  niterations += 1;

  if (!hasCreatedKernels) {
    hasCreatedKernels = true;

    {
      //run CPU version once to estimate sizes
      GaussVol *gvol;
      std::vector<RealVec> positions;
      std::vector<bool> ishydrogen;
      std::vector<RealOpenMM> radii;
      std::vector<RealOpenMM> gammas;
      //outputs
      RealOpenMM volume, vol_energy;
      std::vector<RealOpenMM> free_volume, self_volume;
      std::vector<RealVec> vol_force;
      int numParticles = cl.getNumAtoms();
      //input lists
      positions.resize(numParticles);
      radii.resize(numParticles);
      gammas.resize(numParticles);
      ishydrogen.resize(numParticles);
      //output lists
      free_volume.resize(numParticles);
      self_volume.resize(numParticles);
      vol_force.resize(numParticles);
      double energy_density_param = 4.184*1000.0/27; //about 1 kcal/mol for each water volume
      for (int i = 0; i < numParticles; i++){
	double r, g, alpha, q;
	bool h;
	gvol_force->getParticleParameters(i, r, g, alpha, q, h);
	radii[i] = r;
	gammas[i] = energy_density_param;
	if(h) gammas[i] = 0.0;
	ishydrogen[i] = h;
      }
      gvol = new GaussVol(numParticles, radii, gammas, ishydrogen);
      vector<mm_float4> posq; 
      cl.getPosq().download(posq);
      for(int i=0;i<numParticles;i++){
 	positions[i] = RealVec((RealOpenMM)posq[i].x,(RealOpenMM)posq[i].y,(RealOpenMM)posq[i].z);
      }

      gvol->compute_tree(positions);
      gvol->compute_volume(positions, volume, vol_energy, vol_force, free_volume, self_volume);
      vector<int> noverlaps(cl.getPaddedNumAtoms());
      vector<int> noverlaps_2body(cl.getPaddedNumAtoms());
      for(int i = 0; i<cl.getPaddedNumAtoms(); i++) noverlaps[i] = 0;
      for(int i = 0; i<cl.getPaddedNumAtoms(); i++) noverlaps_2body[i] = 0;
      gvol->getstat(noverlaps, noverlaps_2body);

      //compute maximum number of 2-body overlaps
      int nn = 0, nn2 = 0;
      for(int i = 0; i < noverlaps_2body.size(); i++){
	nn += noverlaps[i];
	nn2 += noverlaps_2body[i];
      }

      if(verbose_level > 0) cout << "Number of overlaps: " << nn << endl;
      if(verbose_level > 0) cout << "Number of 2-body overlaps: " << nn2 << endl;
      
      if(verbose_level > 0){
	cout << "Device: " << cl.getDevice().getInfo<CL_DEVICE_NAME>()  << endl;
	cout << "MaxSharedMem: " << cl.getDevice().getInfo<CL_DEVICE_LOCAL_MEM_SIZE>()  << endl;
	cout << "CompUnits: " << cl.getDevice().getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>()  << endl;
	cout << "Max Work Group Size: " << cl.getDevice().getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>()  << endl;
	cout << "Supports 64bit Atomics: " << useLong << endl;
      }

      bool deviceIsCpu = (cl.getDevice().getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU);
      if(deviceIsCpu){
	// for CPU the force wg size defaults to 1
	ov_work_group_size = 1;
	num_compute_units = nb.getNumForceThreadBlocks(); //cl.getDevice().getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>();
      }else{
	// for GPU use default block size
	ov_work_group_size = nb.getForceThreadBlockSize();
	num_compute_units = nb.getNumForceThreadBlocks();
      }

      //figures out tree sizes, etc.
      int pad_modulo = ov_work_group_size;
      init_tree_size(pad_modulo, noverlaps, noverlaps_2body);

      if(verbose_level > 0) std::cout << "Tree size: " << total_tree_size << std::endl;

      if(verbose_level > 2){
	for(int i = 0; i < num_sections; i++){
	  cout << "Tn: " << i << " " << tree_size[i] << " " << padded_tree_size[i] << " " << natoms_in_tree[i] << " " << tree_pointer[i] << " " << first_atom[i] << endl;
	}
	if(verbose_level > 4){
	  for(int i = 0; i < total_atoms_in_tree; i++){
	    cout << "Atom: " << i << " Slot: " << atom_tree_pointer[i] << endl;
	  }
	}
      }

      if(verbose_level > 0){
	std::cout <<  "Num atoms: " << cl.getNumAtoms() << std::endl;
	std::cout <<  "Padded Num Atoms: " << cl.getPaddedNumAtoms() << std::endl;
	std::cout <<  "Num Atom Blocks: " << cl.getNumAtomBlocks() << std::endl;
	std::cout <<  "Num Tree Sections: " << num_sections << std::endl;
	std::cout <<  "Num Force Buffers: " << nb.getNumForceBuffers() << std::endl;	
	std::cout <<  "Tile size: " << OpenCLContext::TileSize << std::endl;
	std::cout <<  "getNumForceThreadBlocks: " << nb.getNumForceThreadBlocks() << std::endl;
	std::cout <<  "getForceThreadBlockSize: " << nb.getForceThreadBlockSize() << std::endl;
	std::cout <<  "Num Tree Sections: " << num_sections << std::endl;
	std::cout <<  "Work Group Size: " << ov_work_group_size << std::endl;
	std::cout <<  "Tree Size: " <<  total_tree_size << std::endl;

	if(useCutoff){
	  vector<cl_int> icount(1024);
	  nb.getInteractionCount().download(icount);
	  cout << "Using cutoff" << endl;
	  cout << "Number of interacting tiles: " << icount[0] << endl;
	}else{
	  cout << "Not using cutoff" << endl;
	}

      }

      //Sets up buffers

      //atomic buffers
      ovAtomTreePointer = OpenCLArray::create<cl_int>(cl, cl.getPaddedNumAtoms(), "ovAtomTreePointer");

      //tree section buffers
      ovAtomTreeSize = OpenCLArray::create<cl_int>(cl, num_sections, "ovAtomTreeSize");
      NIterations = OpenCLArray::create<cl_int>(cl, num_sections, "NIterations");
      ovAtomTreePaddedSize = OpenCLArray::create<cl_int>(cl, num_sections, "ovAtomTreePaddedSize");
      ovNumAtomsInTree = OpenCLArray::create<cl_int>(cl, num_sections, "ovNumAtomsInTree");
      ovTreePointer = OpenCLArray::create<cl_int>(cl, num_sections, "ovTreePointer");
      ovAtomTreeLock = OpenCLArray::create<cl_int>(cl, num_sections, "ovAtomTreeLock");
      ovFirstAtom = OpenCLArray::create<cl_int>(cl, num_sections, "ovFirstAtom");

      //tree buffers
      ovLevel = OpenCLArray::create<cl_int>(cl, total_tree_size, "ovLevel");
      ovG = OpenCLArray::create<mm_float4>(cl, total_tree_size, "ovG"); //gaussian position + exponent
      ovVolume = OpenCLArray::create<cl_float>(cl, total_tree_size, "ovVolume");
      ovVSfp = OpenCLArray::create<cl_float>(cl, total_tree_size, "ovVSfp");
      ovSelfVolume = OpenCLArray::create<cl_float>(cl, total_tree_size, "ovSelfVolume");
      ovVolEnergy = OpenCLArray::create<cl_float>(cl, total_tree_size, "ovVolEnergy");
      ovGamma1i = OpenCLArray::create<cl_float>(cl, total_tree_size, "ovGamma1i");
      ovDV1 = OpenCLArray::create<mm_float4>(cl, total_tree_size, "ovDV1"); //dV12/dr1 + dV12/dV1 for each overlap
      ovDV2 = OpenCLArray::create<mm_float4>(cl, total_tree_size, "ovDV2"); //volume gradient accumulator
      ovPF = OpenCLArray::create<mm_float4>(cl, total_tree_size, "ovPF"); //(P) and (F) auxiliary variables
      ovLastAtom = OpenCLArray::create<cl_int>(cl, total_tree_size, "ovLastAtom");
      ovRootIndex = OpenCLArray::create<cl_int>(cl, total_tree_size, "ovRootIndex");
      ovChildrenStartIndex = OpenCLArray::create<cl_int>(cl, total_tree_size, "ovChildrenStartIndex");
      ovChildrenCount = OpenCLArray::create<cl_int>(cl, total_tree_size, "ovChildrenCount");
      ovChildrenCountTop = OpenCLArray::create<cl_int>(cl, total_tree_size, "ovChildrenCountTop");
      ovChildrenCountBottom = OpenCLArray::create<cl_int>(cl, total_tree_size, "ovChildrenCountBottom");
      ovProcessedFlag = OpenCLArray::create<cl_int>(cl, total_tree_size, "ovProcessedFlag");
      ovOKtoProcessFlag = OpenCLArray::create<cl_int>(cl, total_tree_size, "ovOKtoProcessFlag");
      ovChildrenReported = OpenCLArray::create<cl_int>(cl, total_tree_size, "ovChildrenReported");

      // atomic reduction buffers, one for each tree section
      // used only if long int atomics are not available
      //   ovAtomBuffer holds volume energy derivatives (in xyz)
      ovAtomBuffer = OpenCLArray::create<mm_float4>(cl, cl.getPaddedNumAtoms()*num_sections, "ovAtomBuffer");
      selfVolumeBuffer = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms()*num_sections, "selfVolumeBuffer");

      // "long" version of  selfVolume accumulation buffer updated using atomics
      selfVolumeBuffer_long = OpenCLArray::create<cl_long>(cl, cl.getPaddedNumAtoms(), "selfVolumeBuffer_long");

      //traditional and "long" versions of general accumulation buffers
      AccumulationBuffer1_real = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms()*num_compute_units, "AccumulationBuffer1_real");
      AccumulationBuffer1_long = OpenCLArray::create<cl_long>(cl, cl.getPaddedNumAtoms(), "AccumulationBuffer1_long");
      AccumulationBuffer2_real = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms()*num_compute_units, "AccumulationBuffer2_real");
      AccumulationBuffer2_long = OpenCLArray::create<cl_long>(cl, cl.getPaddedNumAtoms(), "AccumulationBuffer2_long");


      VdWGBDerForceX = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "VdWGBDerForceX");
      VdWGBDerForceY = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "VdWGBDerForceY");
      VdWGBDerForceZ = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "VdWGBDerForceZ");
      
      // atom-level properties
      selfVolume = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "selfVolume");
      volScalingFactor = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "volScalingFactor");
      BornRadius = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "BornRadius");
      invBornRadius = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "invBornRadius");
      invBornRadius_fp = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "invBornRadius_fp");
      GBDerY = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "GBDerY"); //Y intermediate variable for Born radius-dependent GB derivative
      GBDerBrU = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "GBDerBrU"); //bru variable for Born radius-dependent GB derivative
      GBDerU = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "GBDerU"); //W variable for self-volume-dependent GB derivative
      VdWDerBrW = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "VdWDerBrW"); //brw variable for Born radius-dependent Van der Waals derivative
      VdWDerW = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "VdWDerW"); //W variable for self-volume-dependent vdW derivative

      //atomic parameters
      GaussianExponent = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "GaussianExponent");
      GaussianVolume = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "GaussianVolume");
      AtomicGamma = OpenCLArray::create<cl_float>(cl, cl.getPaddedNumAtoms(), "AtomicGamma");

      //temp buffers to cache intermediate data in overlap tree construction (3-body and up)
      int smax = 64; // this is n*(n-1)/2 where n is the max number of neighbors per overlap
      temp_buffer_size = ov_work_group_size*num_sections*smax;
      gvol_buffer_temp = OpenCLArray::create<cl_float>(cl, temp_buffer_size, "gvol_buffer_temp");
      tree_pos_buffer_temp = OpenCLArray::create<cl_uint>(cl, temp_buffer_size, "tree_pos_buffer_temp");
      i_buffer_temp = OpenCLArray::create<cl_int>(cl, temp_buffer_size, "i_buffer_temp");
      atomj_buffer_temp = OpenCLArray::create<cl_int>(cl, temp_buffer_size, "atomj_buffer_temp");

      //now copy overlap tree arrays to device
      OpenCLCalcAGBNPForceKernel::copy_tree_to_device();
    }

    {
      //Reset tree kernel
      map<string, string> defines;
      defines["FORCE_WORK_GROUP_SIZE"] = cl.intToString(ov_work_group_size);
      defines["NUM_ATOMS"] = cl.intToString(cl.getNumAtoms());
      defines["NUM_ATOMS_TREE"] = cl.intToString(total_atoms_in_tree);
      defines["PADDED_NUM_ATOMS"] = cl.intToString(cl.getPaddedNumAtoms());
      defines["NUM_BLOCKS"] = cl.intToString(num_sections);
      defines["TILE_SIZE"] = cl.intToString(OpenCLContext::TileSize);
      
      map<string, string> replacements;
      string file, kernel_name;
      cl::Program program;
      int index;
      cl::Kernel kernel;

      kernel_name = "resetTree";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      file = cl.replaceStrings(OpenCLAGBNPKernelSources::GVolResetTree, replacements);
      program = cl.createProgram(file, defines);
      //reset tree kernel
      resetTreeKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = resetTreeKernel;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovSelfVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolEnergy->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV2->getDeviceBuffer());

      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenReported->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeLock->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ishydrogenParam->getDeviceBuffer());

      //reset buffer kernel
      kernel_name = "resetBuffer";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      resetBufferKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = resetBufferKernel;
      kernel.setArg<cl_int>(index++, cl.getPaddedNumAtoms());
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovAtomBuffer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, selfVolumeBuffer->getDeviceBuffer());
      if(useLong) kernel.setArg<cl::Buffer>(index++, selfVolumeBuffer_long->getDeviceBuffer());

      
      //reset tree counters kernel
      kernel_name = "resetSelfVolumes";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      resetSelfVolumesKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      kernel = resetSelfVolumesKernel;
      index = 0;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenReported->getDeviceBuffer());
    }


    {
      //Tree construction 
      cl::Program program;
      cl::Kernel kernel;
      string kernel_name;
      int index;

      //pass 1
      map<string, string> pairValueDefines;
      if (useCutoff)
	pairValueDefines["USE_CUTOFF"] = "1";
      if (usePeriodic)
	pairValueDefines["USE_PERIODIC"] = "1";
      pairValueDefines["USE_EXCLUSIONS"] = "1";
      pairValueDefines["FORCE_WORK_GROUP_SIZE"] = cl.intToString(ov_work_group_size);
      pairValueDefines["CUTOFF"] = cl.doubleToString(cutoffDistance);
      pairValueDefines["CUTOFF_SQUARED"] = cl.doubleToString(cutoffDistance*cutoffDistance);
      pairValueDefines["NUM_ATOMS"] = cl.intToString(cl.getNumAtoms());
      pairValueDefines["NUM_ATOMS_TREE"] = cl.intToString(total_atoms_in_tree);
      pairValueDefines["PADDED_NUM_ATOMS"] = cl.intToString(cl.getPaddedNumAtoms());
      pairValueDefines["NUM_BLOCKS"] = cl.intToString(cl.getNumAtomBlocks());
      pairValueDefines["TILE_SIZE"] = cl.intToString(OpenCLContext::TileSize);
      int numExclusionTiles = nb.getExclusionTiles().getSize();
      pairValueDefines["NUM_TILES_WITH_EXCLUSIONS"] = cl.intToString(numExclusionTiles);
      int numContexts = cl.getPlatformData().contexts.size();
      int startExclusionIndex = cl.getContextIndex()*numExclusionTiles/numContexts;
      int endExclusionIndex = (cl.getContextIndex()+1)*numExclusionTiles/numContexts;
      pairValueDefines["FIRST_EXCLUSION_TILE"] = cl.intToString(startExclusionIndex);
      pairValueDefines["LAST_EXCLUSION_TILE"] = cl.intToString(endExclusionIndex);
      pairValueDefines["OV_WORK_GROUP_SIZE"] = cl.intToString(ov_work_group_size);
      pairValueDefines["SMALL_VOLUME"] = "1.e-4";

      map<string, string> replacements;

      replacements["KFC"] = cl.doubleToString((double)KFC);
      replacements["VOLMIN0"] = cl.doubleToString((double)VOLMIN0);
      replacements["VOLMINA"] = cl.doubleToString((double)VOLMINA);
      replacements["VOLMINB"] = cl.doubleToString((double)VOLMINB);
      replacements["MIN_GVOL"] = cl.doubleToString((double)MIN_GVOL);

      replacements["ATOM_PARAMETER_DATA"] = 
	"real4 g; \n"
	"real  v; \n"
	"real  gamma; \n";

      replacements["PARAMETER_ARGUMENTS"] = "";

      replacements["INIT_VARS"] = "";

      replacements["LOAD_ATOM1_PARAMETERS"] = 
	"real a1 = global_gaussian_exponent[atom1]; \n"
	"real v1 = global_gaussian_volume[atom1];\n"
	"real gamma1 = global_atomic_gamma[atom1];\n";

      replacements["LOAD_LOCAL_PARAMETERS_FROM_1"] =
	"localData[localAtomIndex].g.w = a1;\n"
	"localData[localAtomIndex].v = v1;\n"
	"localData[localAtomIndex].gamma = gamma1;\n";


      replacements["LOAD_ATOM2_PARAMETERS"] = 
	"real a2 = localData[localAtom2Index].g.w;\n"
	"real v2 = localData[localAtom2Index].v;\n"
	"real gamma2 = localData[localAtom2Index].gamma;\n";

      replacements["LOAD_LOCAL_PARAMETERS_FROM_GLOBAL"] = 
	"localData[localAtomIndex].g.w = global_gaussian_exponent[j];\n"
	"localData[localAtomIndex].v = global_gaussian_volume[j];\n"
	"localData[localAtomIndex].gamma = global_atomic_gamma[j];\n"
	"localData[localAtomIndex].ov_count = 0;\n";




      //tree locks were used in the 2-body tree construction kernel. no more
      replacements["ACQUIRE_TREE_LOCK"] = "";
      replacements["RELEASE_TREE_LOCK"] = "";

      replacements["COMPUTE_INTERACTION_COUNT"] =
		"	real a12 = a1 + a2; \n"
		"	real deltai = 1./a12; \n"
	        "       real df = a1*a2*deltai; \n"
                "       real ef = exp(-df*r2); \n"
		"	real dfp = df/PI; \n"
		"	real gvol = v1*v2*dfp*dfp*rsqrt(dfp)*ef; \n"
	        "       if(gvol > VolMinA ){ \n" //VolMin0?
	        "          atomic_inc(&ovChildrenCount[atom1_tree_ptr]); \n"
		"       } \n";

      replacements["COMPUTE_INTERACTION_2COUNT"] =
		"	real a12 = a1 + a2; \n"
		"	real deltai = 1./a12; \n"
	        "       real df = a1*a2*deltai; \n"
                "       real ef = exp(-df*r2); \n"
		"	real dfp = df/PI; \n"
		"	real gvol = v1*v2*dfp*dfp*rsqrt(dfp)*ef; \n"
		"	if(gvol > VolMin0 ){ \n"
                "          ov_count += 1; \n"
		"       } \n";

      replacements["COMPUTE_INTERACTION_GVOLONLY"] =
		"	real a12 = a1 + a2; \n"
	        "       real df = a1*a2/a12; \n"
                "       real ef = exp(-df*r2); \n"
		"	real dfp = df/PI; \n"
	        "       real gvol = v1*v2*dfp*dfp*rsqrt(dfp)*ef; \n";


      replacements["COMPUTE_INTERACTION_OTHER"] =
	"         real a12 = a1 + a2; \n"
	"         real df = a1*a2/a12; \n"
	"         real dgvol = -2.0f*df*gvol; \n"
	"         real dgvolv = v1 > 0 ? gvol/v1 : 0; \n" 
	"	  real4 c12 = (a1*posq1 + a2*posq2)/a12; \n"
	"         //switching function \n"
	"         real s = 0, sp = 0; \n"
	"         if(gvol > VolMinB ){ \n"
	"             s = 1.0f; \n"
	"             sp = 0.0f; \n"
	"         }else{ \n"
	"             real swd = 1.f/( VolMinB - VolMinA ); \n"
	"             real swu = (gvol - VolMinA)*swd; \n"
	"             real swu2 = swu*swu; \n"
	"             real swu3 = swu*swu2; \n"
	"             s = swu3*(10.f-15.f*swu+6.f*swu2); \n"
	"             sp = swd*30.f*swu2*(1.f - 2.f*swu + swu2); \n"
	"         }\n"
	"         // switching function end \n"
	"	  real sfp = sp*gvol + s; \n"
	"         gvol = s*gvol; \n";


      replacements["COMPUTE_INTERACTION_STORE1"] =
		"	real a12 = a1 + a2; \n"
		"	real deltai = 1./a12; \n"
	        "       real df = a1*a2*deltai; \n"
                "       real ef = exp(-df*r2); \n"
		"	real dfp = df/PI; \n"
		"	real gvol = v1*v2*dfp*dfp*rsqrt(dfp)*ef; \n"
                "       if(gvol > VolMinA){\n"
                "         real dgvol = -2.0f*df*gvol; \n"
                "         real dgvolv = v1 > 0 ? gvol/v1 : 0; \n" 
		"	  real4 c12 = deltai*(a1*posq1 + a2*posq2); \n"
                "         //switching function \n"
                "         real s = 0, sp = 0; \n"
                "         if(gvol > VolMinB ){ \n"
                "             s = 1.0f; \n"
                "             sp = 0.0f; \n"
                "         }else{ \n"
                "             real swd = 1.f/( VolMinB - VolMinA ); \n"
                "             real swu = (gvol - VolMinA)*swd; \n"
                "             real swu2 = swu*swu; \n"
                "             real swu3 = swu*swu2; \n"
                "             s = swu3*(10.f-15.f*swu+6.f*swu2); \n"
                "             sp = swd*30.f*swu2*(1.f - 2.f*swu + swu2); \n"
	        "         }\n"
                "         // switching function end \n"
                "	  real sfp = sp*gvol + s; \n"
                "         gvol = s*gvol; \n"
		"         /* at this point have:\n"
		"	     1. gvol: overlap  between atom1 and atom2\n"
		"	     2. a12: gaussian exponent of overlap\n"
		"	     3. v12=gvol: volume of overlap\n"
		"	     4. c12: gaussian center of overlap\n"
		"	     These, together with atom2 (last_atom) are entered into the tree for atom 1 if\n"
		"	     volume is large enough.\n"
		"	 */\n"
	        "        int endslot, children_count;\n"
                "        if(gvol > SMALL_VOLUME){ \n"
	        "          //use top counter \n"
	        "          children_count = atomic_inc(&ovChildrenCountTop[atom1_tree_ptr]); \n"
		"          endslot = atom1_children_start + children_count; \n"
                "        }else{ \n"
                "          //use bottom counter \n"
                "          children_count = atomic_inc(&ovChildrenCountBottom[atom1_tree_ptr]); \n"
                "          endslot = atom1_children_start + ovChildrenCount[atom1_tree_ptr] - children_count - 1; \n"
                "        }\n"
		"        ovLevel[endslot] = 2; //two-body\n"
		"	 ovVolume[endslot] = gvol;\n"
	        "        ovVSfp[endslot] = sfp; \n"
		"	 ovGamma1i[endslot] = gamma1 + gamma2;\n"
		"	 ovLastAtom[endslot] = atom2;\n"
		"	 ovRootIndex[endslot] = atom1_tree_ptr;\n"
		"	 ovChildrenStartIndex[endslot] = -1;\n"
		"	 ovChildrenCount[endslot] = 0;\n"
		"	 ovG[endslot] = (real4)(c12.xyz, a12);\n"
	        "        ovDV1[endslot] = (real4)(-delta.xyz*dgvol,dgvolv);\n"
		"      }\n";


      replacements["COMPUTE_INTERACTION_STORE2"] =
		"	real a12 = a1 + a2; \n"
		"	real deltai = 1./a12; \n"
	        "       real df = a1*a2*deltai; \n"
                "       real ef = exp(-df*r2); \n"
		"	real dfp = df/PI; \n"
		"	real gvol = v1*v2*dfp*dfp*rsqrt(dfp)*ef; \n"
                "       if(gvol > VolMinA){\n"
                "         real dgvol = -2.0f*df*gvol; \n"
                "         real dgvolv = v1 > 0 ? gvol/v1 : 0; \n" 
		"	  real4 c12 = deltai*(a1*posq1 + a2*posq2); \n"
                "         //switching function \n"
                "         real s = 0, sp = 0; \n"
                "         if(gvol > VolMinB ){ \n"
                "             s = 1.0f; \n"
                "             sp = 0.0f; \n"
                "         }else{ \n"
                "             real swd = 1.f/( VolMinB - VolMinA ); \n"
                "             real swu = (gvol - VolMinA)*swd; \n"
                "             real swu2 = swu*swu; \n"
                "             real swu3 = swu*swu2; \n"
                "             s = swu3*(10.f-15.f*swu+6.f*swu2); \n"
                "             sp = swd*30.f*swu2*(1.f - 2.f*swu + swu2); \n"
	        "         }\n"
                "         // switching function end \n"
                "	  real sfp = sp*gvol + s; \n"
                "         gvol = s*gvol; \n"
		"	  /* at this point have:\n"
		"	     1. gvol: overlap  between atom1 and atom2\n"
		"	     2. a12: gaussian exponent of overlap\n"
		"	     3. v12=gvol: volume of overlap\n"
		"	     4. c12: gaussian center of overlap\n"
		"	     These, together with atom2 (last_atom) are entered into the tree for atom 1 if\n"
		"	     volume is large enough.\n"
		"	 */\n"
		"	  int endslot = ovChildrenStartIndex[slot] + ov_count; \n"
		"	  ovLevel[endslot] = level + 1; //two-body\n"
		"	  ovVolume[endslot] = gvol;\n"
	        "         ovVSfp[endslot] = sfp; \n"
		"	  ovGamma1i[endslot] = gamma1 + gamma2;\n"
		"	  ovLastAtom[endslot] = atom2;\n"
		"	  ovRootIndex[endslot] = slot;\n"
		"	  ovChildrenStartIndex[endslot] = -1;\n"
		"	  ovChildrenCount[endslot] = 0;\n"
		"	  ovG[endslot] = (real4)(c12.xyz, a12);\n"
	        "         ovDV1[endslot] = (real4)(-delta.xyz*dgvol,dgvolv);\n"
	        "         ovProcessedFlag[endslot] = 0;\n"
                "         ovOKtoProcessFlag[endslot] = 1;\n"
                "         ov_count += 1; \n"
		"       }\n";


      replacements["COMPUTE_INTERACTION_RESCAN"] =
		"	real a12 = a1 + a2; \n"
		"	real deltai = 1./a12; \n"
	        "       real df = a1*a2*deltai; \n"
                "       real ef = exp(-df*r2); \n"
		"	real dfp = df/PI; \n"
		"	real gvol = v1*v2*dfp*dfp*rsqrt(dfp)*ef; \n"
                "       real dgvol = -2.0f*df*gvol; \n"
                "       real dgvolv = v1 > 0 ? gvol/v1 : 0; \n" 
		"       real4 c12 = deltai*(a1*posq1 + a2*posq2); \n"
                "       //switching function \n"
                "       real s = 0, sp = 0; \n"
                "       if(gvol > VolMinB ){ \n"
                "           s = 1.0f; \n"
                "           sp = 0.0f; \n"
                "       }else{ \n"
                "           real swd = 1.f/( VolMinB - VolMinA ); \n"
                "           real swu = (gvol - VolMinA)*swd; \n"
                "           real swu2 = swu*swu; \n"
                "           real swu3 = swu*swu2; \n"
                "           s = swu3*(10.f-15.f*swu+6.f*swu2); \n"
                "           sp = swd*30.f*swu2*(1.f - 2.f*swu + swu2); \n"
	        "       }\n"
                "       // switching function end \n"
                "       real sfp = sp*gvol + s; \n"
                "       gvol = s*gvol; \n"
		"       ovVolume[slot] = gvol;\n"
	        "       ovVSfp[slot] = sfp; \n"
		"       ovG[slot] = (real4)(c12.xyz, a12);\n"
	        "       ovDV1[slot] = (real4)(-delta.xyz*dgvol,dgvolv);\n";



      string InitOverlapTreeSrc = cl.replaceStrings(OpenCLAGBNPKernelSources::GVolOverlapTree, replacements);

      kernel_name = "InitOverlapTree_1body";
      replacements["KERNEL_NAME"] = kernel_name;


      if(verbose) cout << "compiling GVolOverlapTree ..." ;
      program = cl.createProgram(InitOverlapTreeSrc, pairValueDefines);
      if(verbose) cout << " done. " << endl;

      if(verbose) cout << "compiling " << kernel_name << " ... ";
      InitOverlapTreeKernel_1body_1 = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      int reset_tree_size = 1;
      index = 0;
      kernel = InitOverlapTreeKernel_1body_1;
      kernel.setArg<cl_int>(index++, cl.getPaddedNumAtoms());
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl_int>(index++, reset_tree_size);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovNumAtomsInTree->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovFirstAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, NIterations->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, radiusParam1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, gammaParam1->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, ishydrogenParam->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianVolume->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, AtomicGamma->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovG->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());

      if(verbose) cout << "compiling " << kernel_name << " ... ";
      program = cl.createProgram(InitOverlapTreeSrc, pairValueDefines);
      if(verbose) cout << " done. " << endl;
      InitOverlapTreeKernel_1body_2 = cl::Kernel(program, kernel_name.c_str());
      reset_tree_size = 0;
      index = 0;
      kernel = InitOverlapTreeKernel_1body_2;
      kernel.setArg<cl_int>(index++, cl.getPaddedNumAtoms());
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl_int>(index++, reset_tree_size);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovNumAtomsInTree->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovFirstAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, NIterations->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, radiusParam2->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, gammaParam2->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, ishydrogenParam->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianVolume->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, AtomicGamma->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovG->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());

      bool deviceIsCpu = (cl.getDevice().getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU);
      if(deviceIsCpu){
	kernel_name = "InitOverlapTreeCount_cpu";
      }else{
	kernel_name = "InitOverlapTreeCount";
      }
      replacements["KERNEL_NAME"] = kernel_name;

      if(verbose) cout << "compiling " << kernel_name << " ... ";
      InitOverlapTreeCountKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = InitOverlapTreeCountKernel;
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianVolume->getDeviceBuffer() );
      if (useCutoff) {
        kernel.setArg<cl::Buffer>(index++, nb.getInteractingTiles().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getInteractionCount().getDeviceBuffer());
        kernel.setArg<cl_uint>(index++, nb.getInteractingTiles().getSize());
      }else{
	kernel.setArg<cl_uint>(index++, cl.getNumAtomBlocks()*(cl.getNumAtomBlocks()+1)/2);
      }
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());

#ifdef NOTNOW
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianVolume->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, AtomicGamma->getDeviceBuffer() );

      kernel.setArg<cl::Buffer>(index++, nb.getExclusions().getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, nb.getExclusionTiles().getDeviceBuffer());
      if (useCutoff) {
        kernel.setArg<cl::Buffer>(index++, nb.getInteractingTiles().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getInteractionCount().getDeviceBuffer());

	//TO BE FIXED
	cl_float zero(0);
	mm_float4 zero4(zero,zero,zero,zero);
	kernel.setArg<mm_float4>(index++, zero4);
	kernel.setArg<mm_float4>(index++, zero4);
	kernel.setArg<mm_float4>(index++, zero4);
	kernel.setArg<mm_float4>(index++, zero4);
	kernel.setArg<mm_float4>(index++, zero4);

        //index += 5; // The periodic box size arguments are set when the kernel is executed.
        kernel.setArg<cl_uint>(index++, nb.getInteractingTiles().getSize());
        kernel.setArg<cl::Buffer>(index++, nb.getBlockCenters().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getBlockBoundingBoxes().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getInteractingAtoms().getDeviceBuffer());
      }else{
	kernel.setArg<cl_uint>(index++, cl.getNumAtomBlocks()*(cl.getNumAtomBlocks()+1)/2);
      }
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
#endif


      kernel_name = "reduceovCountBuffer";
      replacements["KERNEL_NAME"] = kernel_name;
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      reduceovCountBufferKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = reduceovCountBufferKernel;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCountTop->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCountBottom->getDeviceBuffer());

      if(deviceIsCpu){
	kernel_name = "InitOverlapTree_cpu";
      }else{
	kernel_name = "InitOverlapTree";
      }
      replacements["KERNEL_NAME"] = kernel_name;
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      InitOverlapTreeKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = InitOverlapTreeKernel;
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianVolume->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, AtomicGamma->getDeviceBuffer() );
      if (useCutoff) {
        kernel.setArg<cl::Buffer>(index++, nb.getInteractingTiles().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getInteractionCount().getDeviceBuffer());
        kernel.setArg<cl_uint>(index++, nb.getInteractingTiles().getSize());
      }else{
	kernel.setArg<cl_uint>(index++, cl.getNumAtomBlocks()*(cl.getNumAtomBlocks()+1)/2);
      }
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovG->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCountTop->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCountBottom->getDeviceBuffer());



#ifdef NOTNOW
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeLock->getDeviceBuffer());

      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianVolume->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, AtomicGamma->getDeviceBuffer() );

      kernel.setArg<cl::Buffer>(index++, nb.getExclusions().getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, nb.getExclusionTiles().getDeviceBuffer());
      if (useCutoff) {
        kernel.setArg<cl::Buffer>(index++, nb.getInteractingTiles().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getInteractionCount().getDeviceBuffer());

	//TO BE FIXED
	cl_float zero(0);
	mm_float4 zero4(zero,zero,zero,zero);
	kernel.setArg<mm_float4>(index++, zero4);
	kernel.setArg<mm_float4>(index++, zero4);
	kernel.setArg<mm_float4>(index++, zero4);
	kernel.setArg<mm_float4>(index++, zero4);
	kernel.setArg<mm_float4>(index++, zero4);


        //index += 5; // The periodic box size arguments are set when the kernel is executed.
        kernel.setArg<cl_uint>(index++, nb.getInteractingTiles().getSize());
        kernel.setArg<cl::Buffer>(index++, nb.getBlockCenters().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getBlockBoundingBoxes().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getInteractingAtoms().getDeviceBuffer());
      }else{
	kernel.setArg<cl_uint>(index++, cl.getNumAtomBlocks()*(cl.getNumAtomBlocks()+1)/2);
      }
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovG->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCountTop->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCountBottom->getDeviceBuffer());
#endif

      kernel_name = "resetComputeOverlapTree";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      program = cl.createProgram(InitOverlapTreeSrc, pairValueDefines);
      resetComputeOverlapTreeKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = resetComputeOverlapTreeKernel;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      


      //pass 2
      kernel_name = "ComputeOverlapTree";
      replacements["KERNEL_NAME"] = kernel_name;
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      ComputeOverlapTreeKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = ComputeOverlapTreeKernel;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, NIterations->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeLock->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianVolume->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, AtomicGamma->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovG->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenReported->getDeviceBuffer());

      //pass 2 (1 pass kernel)
      kernel_name = "ComputeOverlapTree_1pass";
      replacements["KERNEL_NAME"] = kernel_name;
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      ComputeOverlapTree_1passKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = ComputeOverlapTree_1passKernel;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, NIterations->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeLock->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianVolume->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, AtomicGamma->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovG->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenReported->getDeviceBuffer());

      kernel.setArg<cl_int>(index++, temp_buffer_size );
      kernel.setArg<cl::Buffer>(index++,  gvol_buffer_temp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++,  tree_pos_buffer_temp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++,  i_buffer_temp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++,  atomj_buffer_temp->getDeviceBuffer());


      //2-body volumes sort kernel
      kernel_name = "SortOverlapTree2body";
      replacements["KERNEL_NAME"] = kernel_name;
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      SortOverlapTree2bodyKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = SortOverlapTree2bodyKernel;
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovG->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());





      //rescan kernels

      kernel_name = "ResetRescanOverlapTree";
      replacements["KERNEL_NAME"] = kernel_name;
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      ResetRescanOverlapTreeKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = ResetRescanOverlapTreeKernel;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());

      kernel_name = "InitRescanOverlapTree";
      replacements["KERNEL_NAME"] = kernel_name;
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      InitRescanOverlapTreeKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = InitRescanOverlapTreeKernel;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());

      //propagates atomic parameters (radii, gammas, etc) from the top to the bottom
      //of the overlap tree, recomputes overlap volumes as it goes 
      kernel_name = "RescanOverlapTree";
      replacements["KERNEL_NAME"] = kernel_name;
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      RescanOverlapTreeKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = RescanOverlapTreeKernel;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, NIterations->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeLock->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, GaussianVolume->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, AtomicGamma->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovG->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenReported->getDeviceBuffer());


      //seeds tree with van der Waals + GB gamma parameters
      kernel_name = "InitOverlapTreeGammas_1body";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      InitOverlapTreeGammasKernel_1body_W = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = InitOverlapTreeGammasKernel_1body_W;
      kernel.setArg<cl_int>(index++, cl.getPaddedNumAtoms());
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovNumAtomsInTree->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovFirstAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, NIterations->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, VdWDerW->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());

      //Same as RescanOverlapTree above:
      //propagates van der Waals + GB gamma atomic parameters from the top to the bottom
      //of the overlap tree,
      //it *does not* recompute overlap volumes
      //  used to prep calculations of volume derivatives of van der Waals energy
      kernel_name = "RescanOverlapTreeGammas";
      replacements["KERNEL_NAME"] = kernel_name;
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      RescanOverlapTreeGammasKernel_W = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = RescanOverlapTreeGammasKernel_W;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, NIterations->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeLock->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, VdWDerW->getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenReported->getDeviceBuffer());


    }


    {
      //Self volumes kernel
      map<string, string> defines;
      defines["FORCE_WORK_GROUP_SIZE"] = cl.intToString(ov_work_group_size);
      defines["NUM_ATOMS_TREE"] = cl.intToString(total_atoms_in_tree);
      defines["NUM_ATOMS"] = cl.intToString(cl.getNumAtoms());
      defines["PADDED_NUM_ATOMS"] = cl.intToString(cl.getPaddedNumAtoms());
      defines["NUM_BLOCKS"] = cl.intToString(cl.getNumAtomBlocks());
      defines["TILE_SIZE"] = cl.intToString(OpenCLContext::TileSize);
      defines["OV_WORK_GROUP_SIZE"] = cl.intToString(ov_work_group_size);
      

      map<string, string> replacements;
      string file = cl.replaceStrings(OpenCLAGBNPKernelSources::GVolSelfVolume, replacements);
      cl::Program program = cl.createProgram(file, defines);

      //accumulates self volumes and volume energy function (and forces)
      //with the energy-per-unit-volume parameters (Gamma1i) currently loaded into tree
      string kernel_name = "computeSelfVolumes";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      computeSelfVolumesKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;      
      cl::Kernel kernel = computeSelfVolumesKernel;
      int index = 0;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, NIterations->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());

      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );

      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovG->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovSelfVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolEnergy->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV2->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovPF->getDeviceBuffer());

      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenReported->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomBuffer->getDeviceBuffer());
      if(useLong){
	kernel.setArg<cl::Buffer>(index++, cl.getLongForceBuffer().getDeviceBuffer());
	kernel.setArg<cl::Buffer>(index++, selfVolumeBuffer_long->getDeviceBuffer());
      }
      kernel.setArg<cl::Buffer>(index++, selfVolumeBuffer->getDeviceBuffer());


      //same as above but w/o updating self volumes
      kernel_name = "computeVolumeEnergy";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      computeVolumeEnergyKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;      
      kernel = computeVolumeEnergyKernel;
      index = 0;
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreeSize->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, NIterations->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePaddedSize->getDeviceBuffer());

      kernel.setArg<cl::Buffer>(index++, GaussianExponent->getDeviceBuffer() );

      kernel.setArg<cl::Buffer>(index++, ovLevel->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVSfp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovGamma1i->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovG->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolEnergy->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovDV2->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovPF->getDeviceBuffer());

      kernel.setArg<cl::Buffer>(index++, ovLastAtom->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovRootIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenStartIndex->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenCount->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovProcessedFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovOKtoProcessFlag->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovChildrenReported->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomBuffer->getDeviceBuffer());
      if(useLong){
	kernel.setArg<cl::Buffer>(index++, cl.getLongForceBuffer().getDeviceBuffer());
      }

    }

    {
      //Self volumes reduction kernel (pass 2)
      map<string, string> defines;
      defines["FORCE_WORK_GROUP_SIZE"] = cl.intToString(ov_work_group_size);
      defines["NUM_ATOMS_TREE"] = cl.intToString(total_atoms_in_tree);
      defines["NUM_ATOMS"] = cl.intToString(cl.getNumAtoms());
      defines["PADDED_NUM_ATOMS"] = cl.intToString(cl.getPaddedNumAtoms());
      defines["NUM_BLOCKS"] = cl.intToString(cl.getNumAtomBlocks());
      defines["TILE_SIZE"] = cl.intToString(OpenCLContext::TileSize);
      defines["NTILES_IN_BLOCK"] = "1";//cl.intToString(ov_work_group_size/OpenCLContext::TileSize);

      map<string, string> replacements;
      string kernel_name = "reduceSelfVolumes_buffer";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      //cout << "compiling " << kernel_name << endl;

      string file = OpenCLAGBNPKernelSources::GVolReduceTree;
      cl::Program program = cl.createProgram(file, defines);
      if(verbose) cout << " done. " << endl;
      reduceSelfVolumesKernel_buffer = cl::Kernel(program, kernel_name.c_str());
      cl::Kernel kernel = reduceSelfVolumesKernel_buffer;
      int index = 0;
      kernel.setArg<cl_int>(index++, cl.getPaddedNumAtoms());
      kernel.setArg<cl_int>(index++, num_sections);
      kernel.setArg<cl::Buffer>(index++, ovAtomTreePointer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovAtomBuffer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ovVolEnergy->getDeviceBuffer());
      if(useLong) kernel.setArg<cl::Buffer>(index++, selfVolumeBuffer_long->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, selfVolumeBuffer->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, selfVolume->getDeviceBuffer());

      int update_energy = 1;
      kernel.setArg<cl_int>(index++, update_energy);
      kernel.setArg<cl::Buffer>(index++, (useLong ? cl.getLongForceBuffer().getDeviceBuffer() : cl.getForceBuffers().getDeviceBuffer())); //master force buffer      
      kernel.setArg<cl::Buffer>(index++, cl.getEnergyBuffer().getDeviceBuffer());
    }

#ifdef NOTNOW
    {//kernel to test hash table to find i4 lookup table based on radii combinations
      //upload hash table
      AGBNPHtable *h_table = i4_lut->get_h_table();
      h_table->h_print();
      unsigned int hsize = h_table->hsize;
      unsigned int hmask = h_table->hmask;
      unsigned int hjump = h_table->hjump;
      i4_hash_values = OpenCLArray::create<cl_int>(cl, hsize, "i4_hash_values");
      i4_hash_values->upload(h_table->values);

      // get list of radii
      unsigned int nradii = 0;
      float Ri;
      int iat = 1;
      vector<float>radii;
      for (int i = 0; i < numParticles; i++) {
	double radius, gamma, alpha, q;
	bool h;
 	gvol_force->getParticleParameters(i, radius, gamma, alpha, q, h);
	if(i == iat){
	  Ri = radius;
	}else if(!h) {
	  radii.push_back(radius);
	  nradii += 1;
	}
      }

      max_num_search_values = nradii;
      Rjs = OpenCLArray::create<cl_float>(cl, nradii, "Rjs");
      ids_of_values = OpenCLArray::create<cl_int>(cl, nradii, "ids_of_values");
      Rjs->upload(radii);

      //hash table kernel
      map<string, string> defines;
      defines["NUM_ATOMS"] = cl.intToString(cl.getNumAtoms());
      defines["AGBNP_RADIUS_INCREMENT"] = cl.doubleToString(AGBNP_RADIUS_INCREMENT);
      defines["AGBNP_RADIUS_PRECISION"] = cl.intToString(AGBNP_RADIUS_PRECISION);

      map<string, string> replacements;
      string file, kernel_name;
      cl::Program program;
      int index;
      cl::Kernel kernel;

      kernel_name = "testHash";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      file = cl.replaceStrings(OpenCLAGBNPKernelSources::AGBNPBornRadii, replacements);
      program = cl.createProgram(file, defines);
      //testHash kernel
      testHashKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = testHashKernel;
      kernel.setArg<cl_uint>(index++, h_table->hsize);
      kernel.setArg<cl_uint>(index++, h_table->hmask);
      kernel.setArg<cl_uint>(index++, h_table->hjump);
      kernel.setArg<cl::Buffer>(index++, i4_hash_values->getDeviceBuffer());
      kernel.setArg<cl_uint>(index++, nradii);
      kernel.setArg<cl_float>(index++, Ri);
      kernel.setArg<cl::Buffer>(index++, Rjs->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ids_of_values->getDeviceBuffer());
    }


    {
      int itable = 24;
      int num_values = 4*i4_table_size;
      testF = OpenCLArray::create<cl_float>(cl, num_values, "testF");
      testDerF = OpenCLArray::create<cl_float>(cl, num_values, "testDerF");

      //hash table kernel
      map<string, string> defines;
      defines["NUM_ATOMS"] = cl.intToString(cl.getNumAtoms());
      defines["PADDED_NUM_ATOMS"] = cl.intToString(cl.getPaddedNumAtoms());
      defines["AGBNP_RADIUS_INCREMENT"] = cl.doubleToString(AGBNP_RADIUS_INCREMENT);
      defines["AGBNP_RADIUS_PRECISION"] = cl.intToString(AGBNP_RADIUS_PRECISION);

      map<string, string> replacements;
      string file, kernel_name;
      cl::Program program;
      int index;
      cl::Kernel kernel;

      kernel_name = "testLookup";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      file = cl.replaceStrings(OpenCLAGBNPKernelSources::AGBNPBornRadii, replacements);
      program = cl.createProgram(file, defines);
      // testLookup kernel
      testLookupKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = testLookupKernel;
      kernel.setArg<cl_int>(index++, i4_table_size);
      kernel.setArg<cl_float>(index++, i4_rmin);
      kernel.setArg<cl_float>(index++, i4_rmax);
      kernel.setArg<cl::Buffer>(index++, i4YValues->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, i4Y2Values->getDeviceBuffer());
      kernel.setArg<cl_int>(index++, itable);
      kernel.setArg<cl_int>(index++, num_values);
      kernel.setArg<cl::Buffer>(index++, testF->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, testDerF->getDeviceBuffer());
    }
#endif
    
    {
      //Born radii initialization and calculation kernels
      map<string, string> defines;
      defines["NUM_ATOMS"] = cl.intToString(cl.getNumAtoms());
      defines["PADDED_NUM_ATOMS"] = cl.intToString(cl.getPaddedNumAtoms());
      defines["AGBNP_RADIUS_INCREMENT"] = cl.doubleToString(AGBNP_RADIUS_INCREMENT);
      defines["AGBNP_HB_RADIUS"] = cl.doubleToString(AGBNP_HB_RADIUS);
      defines["AGBNP_RADIUS_PRECISION"] = cl.intToString(AGBNP_RADIUS_PRECISION);
      defines["TILE_SIZE"] = cl.intToString(OpenCLContext::TileSize);
      defines["FORCE_WORK_GROUP_SIZE"] = cl.intToString(ov_work_group_size);
      if (useCutoff)
	defines["USE_CUTOFF"] = "1";
      defines["NUM_BLOCKS"] = cl.intToString(cl.getNumAtomBlocks());
      defines["AGBNP_I4LOOKUP_MAXA"] = cl.intToString(AGBNP_I4LOOKUP_MAXA);

      map<string, string> replacements;
      replacements["INIT_VARS"] = "";

      string file, kernel_name;
      cl::Program program;
      int index;
      cl::Kernel kernel;

      if(verbose) cout << "compiling AGBNPBornRadii.cl" << " ... ";
      file = cl.replaceStrings(OpenCLAGBNPKernelSources::AGBNPBornRadii, replacements);
      program = cl.createProgram(file, defines);

      //initBornRadii kernel
      kernel_name = "initBornRadii";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      initBornRadiiKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = initBornRadiiKernel;
      kernel.setArg<cl_uint>(index++, cl.getPaddedNumAtoms()); //bufferSize
      kernel.setArg<cl_uint>(index++, num_compute_units);     //numBuffers
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_long->getDeviceBuffer()); //invBornRadiusBuffer_long
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_real->getDeviceBuffer()); //invBornRadiusBuffer
      kernel.setArg<cl::Buffer>(index++, radiusParam1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, selfVolume->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, volScalingFactor->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, invBornRadius->getDeviceBuffer());

      bool deviceIsCpu = (cl.getDevice().getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU);
      if(deviceIsCpu){
	kernel_name = "inverseBornRadii_cpu";
      }else{
	kernel_name = "inverseBornRadii";
      }
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      //inverseBornRadii kernel
      inverseBornRadiiKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = inverseBornRadiiKernel;
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, radiusParam1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, volScalingFactor->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ishydrogenParam->getDeviceBuffer() );
      //neighbor list
      if (useCutoff) {
        kernel.setArg<cl::Buffer>(index++, nb.getInteractingTiles().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getInteractionCount().getDeviceBuffer());
        kernel.setArg<cl_uint>(index++, nb.getInteractingTiles().getSize());
      }else{
	kernel.setArg<cl_uint>(index++, cl.getNumAtomBlocks()*(cl.getNumAtomBlocks()+1)/2);
      }
      //dictionary of table indexes
      kernel.setArg<cl_uint>(index++, hsize);
      kernel.setArg<cl_uint>(index++, hmask);
      kernel.setArg<cl_uint>(index++, hjump);
      kernel.setArg<cl::Buffer>(index++, i4_hash_values->getDeviceBuffer());
      //spline lookup tables
      kernel.setArg<cl_int>(index++, i4_table_size);
      kernel.setArg<cl_float>(index++, i4_rmin);
      kernel.setArg<cl_float>(index++, i4_rmax);
      kernel.setArg<cl::Buffer>(index++, i4YValues->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, i4Y2Values->getDeviceBuffer());
      //accumulation buffers for inverse Born radii
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_long->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_real->getDeviceBuffer());


      kernel_name = "reduceBornRadii";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      //reduceBornRadii kernel
      reduceBornRadiiKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = reduceBornRadiiKernel;
      kernel.setArg<cl_uint>(index++, cl.getPaddedNumAtoms()); //bufferSize
      kernel.setArg<cl_uint>(index++, num_compute_units);     //numBuffers
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_long->getDeviceBuffer()); //invBornRadiusBuffer_long
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_real->getDeviceBuffer()); //invBornRadiusBuffer
      kernel.setArg<cl::Buffer>(index++, radiusParam1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, invBornRadius->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, invBornRadius_fp->getDeviceBuffer()); //derivative of filter function
      kernel.setArg<cl::Buffer>(index++, BornRadius->getDeviceBuffer());


      kernel_name = "VdWEnergy";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      //reduceBornRadii kernel
      VdWEnergyKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = VdWEnergyKernel;
      kernel.setArg<cl::Buffer>(index++, alphaParam->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, BornRadius->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, invBornRadius_fp->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, VdWDerBrW->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, cl.getEnergyBuffer().getDeviceBuffer()); //master energy buffer
      kernel.setArg<cl::Buffer>(index++, testBuffer->getDeviceBuffer()); //VDw Energy for testing
      

      kernel_name = "initVdWGBDerBorn";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      //initVdWGBDerBorn kernel
      initVdWGBDerBornKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = initVdWGBDerBornKernel;
      kernel.setArg<cl_uint>(index++, cl.getPaddedNumAtoms()); //bufferSize
      kernel.setArg<cl_uint>(index++, num_compute_units);     //numBuffers
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_long->getDeviceBuffer()); //VdWDerWBuffer_long
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_real->getDeviceBuffer()); //VdWDerWBuffer
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_long->getDeviceBuffer()); //GBDerUBuffer_long
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_real->getDeviceBuffer()); //GBDerUBuffer

      
      //components of the derivatives of the van der Waals and GB energy functions due
      //variations of Born Radii. This kernel mirrors inverseBornRadii kernel above. Both
      //perform a pair calculation of the descreening function. 
      if(deviceIsCpu){
	kernel_name = "VdWGBDerBorn_cpu";
      }else{
	kernel_name = "VdWGBDerBorn";
      }
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      // VdWGBDerBorn kernel
      VdWGBDerBornKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = VdWGBDerBornKernel;
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, radiusParam1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, volScalingFactor->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, ishydrogenParam->getDeviceBuffer() );
      //neighbor list
      if (useCutoff) {
        kernel.setArg<cl::Buffer>(index++, nb.getInteractingTiles().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getInteractionCount().getDeviceBuffer());
        kernel.setArg<cl_uint>(index++, nb.getInteractingTiles().getSize());
      }else{
	kernel.setArg<cl_uint>(index++, cl.getNumAtomBlocks()*(cl.getNumAtomBlocks()+1)/2);
      }
      //dictionary of table indexes
      kernel.setArg<cl_uint>(index++, hsize);
      kernel.setArg<cl_uint>(index++, hmask);
      kernel.setArg<cl_uint>(index++, hjump);
      kernel.setArg<cl::Buffer>(index++, i4_hash_values->getDeviceBuffer());
      //spline lookup tables
      kernel.setArg<cl_int>(index++, i4_table_size);
      kernel.setArg<cl_float>(index++, i4_rmin);
      kernel.setArg<cl_float>(index++, i4_rmax);
      kernel.setArg<cl::Buffer>(index++, i4YValues->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, i4Y2Values->getDeviceBuffer());
      //BrW and BrU intermediate parameters
      kernel.setArg<cl::Buffer>(index++, VdWDerBrW->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, GBDerBrU->getDeviceBuffer());
      //accumulation buffers for U and W variables for volume component of the derivatives of the GB and vdw energy functions
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_long->getDeviceBuffer()); //VdWDerWBuffer_long
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_real->getDeviceBuffer()); //VdWDerWBuffer_long
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_long->getDeviceBuffer()); //GBDerUBuffer_long
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_real->getDeviceBuffer()); //GBDerUBuffer
      kernel.setArg<cl::Buffer>(index++, (useLong ? cl.getLongForceBuffer().getDeviceBuffer() : cl.getForceBuffers().getDeviceBuffer())); //master force buffer      
     

      kernel_name = "reduceVdWGBDerBorn";
      if(verbose) cout << "compiling " << kernel_name << " ... ";
      //reduceVdWGBDerBorn kernel
      reduceVdWGBDerBornKernel = cl::Kernel(program, kernel_name.c_str());
      if(verbose) cout << " done. " << endl;
      index = 0;
      kernel = reduceVdWGBDerBornKernel;
      kernel.setArg<cl_uint>(index++, cl.getPaddedNumAtoms()); //bufferSize
      kernel.setArg<cl_uint>(index++, num_compute_units);     //numBuffers
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_long->getDeviceBuffer()); //VdWDerWBuffer
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_real->getDeviceBuffer()); 
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_long->getDeviceBuffer()); //GBDerUBuffer
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_real->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, radiusParam1->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, VdWDerW->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, GBDerU->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++,VdWGBDerForceX->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++,VdWGBDerForceY->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++,VdWGBDerForceZ->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, (useLong ? cl.getLongForceBuffer().getDeviceBuffer() : cl.getForceBuffers().getDeviceBuffer())); //master force buffer      
    }

    {
      //GB Energy kernels
   
      double dielectric_in = 1.0;
      double dielectric_out= 80.0;
      double tokjmol = 4.184*332.0/10.0; //the factor of 10 is the conversion of 1/r from nm to Ang
      double dielectric_factor = tokjmol*(-0.5)*(1./dielectric_in - 1./dielectric_out);

      map<string, string> defines;
      defines["NUM_ATOMS"] = cl.intToString(cl.getNumAtoms());
      defines["PADDED_NUM_ATOMS"] = cl.intToString(cl.getPaddedNumAtoms());
      defines["TILE_SIZE"] = cl.intToString(OpenCLContext::TileSize);
      defines["FORCE_WORK_GROUP_SIZE"] = cl.intToString(ov_work_group_size);
      if (useCutoff)
	defines["USE_CUTOFF"] = "1";
      defines["NUM_BLOCKS"] = cl.intToString(cl.getNumAtomBlocks());
      defines["AGBNP_DIELECTRIC_FACTOR"] = cl.doubleToString(dielectric_factor);

      map<string, string> replacements;
      replacements["INIT_VARS"] = "";

      string file, kernel_name;
      cl::Program program;
      int index;
      cl::Kernel kernel;

      if(verbose) cout << "compiling AGBNPGBEnergy.cl ... ";
      file = cl.replaceStrings(OpenCLAGBNPKernelSources::AGBNPGBEnergy, replacements);
      program = cl.createProgram(file, defines);
      if(verbose) cout << " done. " << endl;

      //initGBEnergy kernel
      kernel_name = "initGBEnergy";
      initGBEnergyKernel = cl::Kernel(program, kernel_name.c_str());
      index = 0;
      kernel = initGBEnergyKernel;
      kernel.setArg<cl_uint>(index++, cl.getPaddedNumAtoms()); //bufferSize
      kernel.setArg<cl_uint>(index++, num_compute_units);     //numBuffers
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_long->getDeviceBuffer()); //GB Energy buffer (long)
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_real->getDeviceBuffer()); //GB energy buffer
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_long->getDeviceBuffer()); //Y buffer (long)
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_real->getDeviceBuffer()); //Y

      //GBPairEnergy kernel
      bool deviceIsCpu = (cl.getDevice().getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU);
      if(deviceIsCpu){
	kernel_name = "GBPairEnergy_cpu";
      }else{
	kernel_name = "GBPairEnergy";
      }
      GBPairEnergyKernel = cl::Kernel(program, kernel_name.c_str());
      index = 0;
      kernel = GBPairEnergyKernel;
      kernel.setArg<cl::Buffer>(index++, cl.getPosq().getDeviceBuffer() );
      kernel.setArg<cl::Buffer>(index++, chargeParam->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, BornRadius->getDeviceBuffer());
      //neighbor list
      if (useCutoff) {
        kernel.setArg<cl::Buffer>(index++, nb.getInteractingTiles().getDeviceBuffer());
        kernel.setArg<cl::Buffer>(index++, nb.getInteractionCount().getDeviceBuffer());
        kernel.setArg<cl_uint>(index++, nb.getInteractingTiles().getSize());
      }else{
	kernel.setArg<cl_uint>(index++, cl.getNumAtomBlocks()*(cl.getNumAtomBlocks()+1)/2);
      }
      //accumulation buffers for GB Energy
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_long->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_real->getDeviceBuffer());
      //accumulation buffers for "Y" parameters
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_long->getDeviceBuffer()); //Y buffer (long)
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_real->getDeviceBuffer()); //Y buffer
      kernel.setArg<cl::Buffer>(index++, (useLong ? cl.getLongForceBuffer().getDeviceBuffer() : cl.getForceBuffers().getDeviceBuffer())); //master force buffer      
      kernel.setArg<cl::Buffer>(index++, cl.getEnergyBuffer().getDeviceBuffer()); //master energy buffer
      
      //GB energy reduction kernel
      kernel_name = "reduceGBEnergy";
      reduceGBEnergyKernel = cl::Kernel(program, kernel_name.c_str());
      index = 0;
      kernel = reduceGBEnergyKernel;
      kernel.setArg<cl_uint>(index++, cl.getPaddedNumAtoms()); //bufferSize
      kernel.setArg<cl_uint>(index++, num_compute_units);     //numBuffers
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_long->getDeviceBuffer()); //GB Energy buffer (long)
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer1_real->getDeviceBuffer()); //GB Energy buffer
      kernel.setArg<cl::Buffer>(index++, chargeParam->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, BornRadius->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, invBornRadius_fp->getDeviceBuffer());
      if(useLong) kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_long->getDeviceBuffer()); //Y buffer (long)
      kernel.setArg<cl::Buffer>(index++, AccumulationBuffer2_real->getDeviceBuffer()); //Y buffer
      kernel.setArg<cl::Buffer>(index++, GBDerY->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, GBDerBrU->getDeviceBuffer());
      kernel.setArg<cl::Buffer>(index++, cl.getEnergyBuffer().getDeviceBuffer()); //master energy buffer
    }


  }

  //------------------------------------------------------------------------------------------------------------
  // Tree construction
  //  
  if(verbose_level > 2) cout << "Executing resetTreeKernel" << endl;
  //here workgroups cycle through tree sections to reset the tree section
  cl.executeKernel(resetTreeKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing resetBufferKernel" << endl;
  // resets either ovAtomBuffer and long energy buffer
  cl.executeKernel(resetBufferKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing InitOverlapTreeKernel_1body_1" << endl;
  //fills up tree with 1-body overlaps
  cl.executeKernel(InitOverlapTreeKernel_1body_1, ov_work_group_size*num_compute_units, ov_work_group_size);
  
  if(verbose_level > 2) cout << "Executing InitOverlapTreeCountKernel" << endl;
  // compute numbers of 2-body overlaps, that is children counts of 1-body overlaps
  cl.executeKernel(InitOverlapTreeCountKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing reduceovCountBufferKernel" << endl;
  // do a prefix sum of 2-body counts to compute children start indexes to store 2-body overlaps computed by InitOverlapTreeKernel below
  cl.executeKernel(reduceovCountBufferKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing InitOverlapTreeKernel" << endl;
  cl.executeKernel(InitOverlapTreeKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing resetComputeOverlapTreeKernel" << endl;
  cl.executeKernel(resetComputeOverlapTreeKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing ComputeOverlapTree_1passKernel" << endl;
  cl.executeKernel(ComputeOverlapTree_1passKernel, ov_work_group_size*num_compute_units, ov_work_group_size);
  //------------------------------------------------------------------------------------------------------------




  

  //------------------------------------------------------------------------------------------------------------
  // Self volumes, volume scaling parameters, and volume energy function 1
  //
  if(verbose_level > 2) cout << "Executing resetSelfVolumesKernel" << endl;
  cl.executeKernel(resetSelfVolumesKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing computeSelfVolumesKernel" << endl;
  cl.executeKernel(computeSelfVolumesKernel, ov_work_group_size*num_compute_units, ov_work_group_size);


  if(verbose_level > 2) cout << "Executing reduceSelfVolumesKernel_buffer" << endl;
  cl.executeKernel(reduceSelfVolumesKernel_buffer, ov_work_group_size*num_compute_units, ov_work_group_size);





  
  if(verbose){
    //print self volumes
    vector<float> self_volumes(cl.getPaddedNumAtoms());
    selfVolume->download(self_volumes);
    for(int i=0;i<numParticles;i++){
      cout << "self_volume:" << i << "  " << self_volumes[i] << endl;
    }
  }

  if(verbose){
    vector<int> atom_pointer(cl.getPaddedNumAtoms());
    vector<float> vol_energies(total_tree_size);
    ovAtomTreePointer->download(atom_pointer);
    ovVolEnergy->download(vol_energies);
    double energy = 0;
    for(int i=0;i<numParticles;i++){
      int slot = atom_pointer[i];
      energy += vol_energies[slot];
    }
    cout << "Volume Energy 1:" << energy << endl;
  }


  //------------------------------------------------------------------------------------------------------------

#ifdef NOTNOW
  if(verbose_level > 2) cout << "Executing testLookupKernel" << endl;
  cl.executeKernel(testLookupKernel, ov_work_group_size*num_compute_units, ov_work_group_size);
  
  if(verbose){
    // print lookup table results
    vector<float>f(4*i4_table_size);
    vector<float>derf(4*i4_table_size);
    testF->download(f);
    testDerF->download(derf);
    float dx = 0.03;
    float x = i4_rmin;
    for(int i=0;i<4*i4_table_size;i++){
      cout << "lut: " << x << " " << f[i] << " " << derf[i] << endl;
      x += dx;
    }
  }
#endif

  //------------------------------------------------------------------------------------------------------------
  // Born radii
  //
  if(verbose_level > 2) cout << "Executing initBornRadiiKernel" << endl;
  cl.executeKernel(initBornRadiiKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing inverseBornRadiiKernel" << endl;
  cl.executeKernel(inverseBornRadiiKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose && !useLong){
    vector<float> inv_br_buffer(cl.getPaddedNumAtoms()*num_compute_units);
    AccumulationBuffer1_real->download(inv_br_buffer);
    for(int cu=0;cu<num_compute_units;cu++){
      for(int iatom = 0; iatom < cl.getPaddedNumAtoms(); iatom++){
	cout << "BR_buff: " << cu << " " << iatom << " " << inv_br_buffer[cl.getPaddedNumAtoms()*cu + iatom] << endl;
      }
    }
  }
  
  if(verbose_level > 2) cout << "Executing reduceBornRadiiKernel" << endl;
  cl.executeKernel(reduceBornRadiiKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose){
    // prints out Born radii
    vector<float> born_radii(cl.getPaddedNumAtoms());
    vector<float> sf(cl.getPaddedNumAtoms());
    BornRadius->download(born_radii);
    volScalingFactor->download(sf);
    for(int i=0;i<numParticles;i++){
      double radius, gamma, alpha, charge;
      bool ishydrogen;
      gvol_force->getParticleParameters(i, radius, gamma, alpha, charge, ishydrogen);
      cout << "born_radius: " << i << "  " << born_radii[i] << "  " << (radius - AGBNP_RADIUS_INCREMENT) << "  " << sf[i] << endl;
    }
  }    
  //------------------------------------------------------------------------------------------------------------

  //------------------------------------------------------------------------------------------------------------
  //Van der Waals dispersion energy function
  //
  //------------------------------------------------------------------------------------------------------------
  if(verbose_level > 2) cout << "Executing vdwEnergyKernel" << endl;
  cl.executeKernel(VdWEnergyKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose){
    // get the VdW energy (sum over the atoms in the buffer)
    vector<float> vdw_energies(cl.getPaddedNumAtoms());
    testBuffer->download(vdw_energies);
    double vdw_energy = 0.0;
    for(int i=0; i<numParticles; i++){
      cout << "EVdW: " << i << " " << vdw_energies[i] << endl;
      vdw_energy += vdw_energies[i];
    }
    cout << "VdW Energy: " << vdw_energy << endl;
  }


  if(verbose){
    // get the BrU parameters
    vector<float> brw_params(cl.getPaddedNumAtoms());
    VdWDerBrW->download(brw_params);
    for(int i=0; i<numParticles; i++){
      cout << "BrW: " << i << " " << brw_params[i] << endl;
    }
  }


  
  //------------------------------------------------------------------------------------------------------------
  //GB energy function
  //
  if(verbose_level > 2) cout << "Executing initGBEnergyKernel" << endl;
  cl.executeKernel(initGBEnergyKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing GBPairEnergyKernel" << endl;
  cl.executeKernel(GBPairEnergyKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing reduceGBEnergyKernel" << endl;
  cl.executeKernel(reduceGBEnergyKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose){
    // get the GB energy (sum over the atoms in the buffer)
    vector<float> gb_energies(cl.getPaddedNumAtoms()*num_compute_units);
    AccumulationBuffer1_real->download(gb_energies);
    double gb_energy = 0.0;
    for(int i=0; i<numParticles; i++){
      cout << "EGB: " << i << " " << gb_energies[i] << endl;
      gb_energy += gb_energies[i];
    }
    cout << "GB Energy: " << gb_energy << endl;
  }

  if(verbose){
    // get the Y parameters
    vector<float> y_params(cl.getPaddedNumAtoms());
    GBDerY->download(y_params);
    for(int i=0; i<numParticles; i++){
      cout << "Y: " << i << " " << y_params[i] << endl;
    }
  }

  if(verbose){
    // get the BrU parameters
    vector<float> bru_params(cl.getPaddedNumAtoms());
    GBDerBrU->download(bru_params);
    for(int i=0; i<numParticles; i++){
      cout << "BrU: " << i << " " << bru_params[i] << endl;
    }
  }

  //------------------------------------------------------------------------------------------------------------



  //------------------------------------------------------------------------------------------------------------
  //Born-radii related derivatives
  //
  if(verbose_level > 2) cout << "Executing VdWGBDerBornKernel" << endl;
  cl.executeKernel(initVdWGBDerBornKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing VdWGBDerBornKernel" << endl;
  cl.executeKernel(VdWGBDerBornKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing reduceVdWGBDerBornKernel" << endl;
  cl.executeKernel(reduceVdWGBDerBornKernel, ov_work_group_size*num_compute_units, ov_work_group_size);


  if(verbose){
    // get the U parameters
    vector<float> u_params(cl.getPaddedNumAtoms());
    GBDerU->download(u_params);
    for(int i=0; i<numParticles; i++){
      cout << "U: " << i << " " << u_params[i] << endl;
    }
  }


  if(verbose){
    // get the W parameters
    vector<float> w_params(cl.getPaddedNumAtoms());
    VdWDerW->download(w_params);
    for(int i=0; i<numParticles; i++){
      cout << "W: " << i << " " << w_params[i] << endl;
    }
  }

  if(verbose){
    // get VdW forces
    vector<float> xforce(cl.getPaddedNumAtoms());
    vector<float> yforce(cl.getPaddedNumAtoms());
    vector<float> zforce(cl.getPaddedNumAtoms());
    VdWGBDerForceX->download(xforce);
    VdWGBDerForceY->download(yforce);
    VdWGBDerForceZ->download(zforce);
    double offset = 1.e-3;
    int pmove = 107;
    int direction = 1;
    double f;
    if(direction == 0) f = xforce[pmove];
    if(direction == 1) f = yforce[pmove];
    if(direction == 2) f = zforce[pmove];
    double de = -f*offset;
    cout << "Force (AGBNP):" << f << endl ;
    cout << "deVdw (no volumes, from force) = " << de << endl;
  }
  
  
  //------------------------------------------------------------------------------------------------------------
  //Van der Waals and GB "volume" derivatives
  //

  //seeds the top of the tree with van der Waals + GB gamma parameters
  if(verbose_level > 2) cout << "Executing InitOverlapTreeGammasKernel_1body_W " << endl;
  cl.executeKernel(InitOverlapTreeGammasKernel_1body_W, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing ResetRescanOverlapTreeKernel " << endl;
  cl.executeKernel(ResetRescanOverlapTreeKernel, ov_work_group_size*num_compute_units, ov_work_group_size);
  
  if(verbose_level > 2) cout << "Executing InitRescanOverlapTreeKernel " << endl;
  cl.executeKernel(InitRescanOverlapTreeKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  //propagates gamma atomic parameters from the top to the bottom
  //of the overlap tree
  if(verbose_level > 2) cout << "Executing RescanOverlapTreeGammasKernel " << endl;
  cl.executeKernel(RescanOverlapTreeGammasKernel_W, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing resetSelfVolumesKernel" << endl;
  cl.executeKernel(resetSelfVolumesKernel, ov_work_group_size*num_compute_units, ov_work_group_size);
  
  //collect derivatives from volume energy function with van der Waals gamma parameters
  if(verbose_level > 2) cout << "Executing computeVolumeEnergyKernel " << endl;
  cl.executeKernel(computeVolumeEnergyKernel, ov_work_group_size*num_compute_units, ov_work_group_size);


  if(verbose_level > 4){
    float self_volume = 0.0;
    vector<cl_float> self_volumes(total_tree_size);
    vector<cl_float> volumes(total_tree_size);
    vector<cl_float> energies(total_tree_size);
    vector<cl_float> gammas(total_tree_size);
    vector<cl_int> last_atom(total_tree_size);
    vector<cl_int> level(total_tree_size);
    vector<cl_int> parent(total_tree_size);
    vector<cl_int> children_start_index(total_tree_size);
    vector<cl_int> children_count(total_tree_size);
    vector<cl_int> children_reported(total_tree_size);
    vector<mm_float4> g(total_tree_size);
    vector<mm_float4> dv2(total_tree_size);
    vector<mm_float4> dv1(total_tree_size);
    vector<cl_float> sfp(total_tree_size);
    vector<int> size(num_sections);
    vector<int> tree_pointer_t(num_sections);
    vector<cl_int> processed(total_tree_size);
    vector<cl_int> oktoprocess(total_tree_size);


    ovSelfVolume->download(self_volumes);
    ovVolume->download(volumes);
    ovVolEnergy->download(energies);
    ovLevel->download(level);
    ovLastAtom->download(last_atom);
    ovRootIndex->download(parent);
    ovChildrenStartIndex->download(children_start_index);
    ovChildrenCount->download(children_count);
    ovChildrenReported->download(children_reported);
    ovG->download(g);
    ovGamma1i->download(gammas);
    ovDV1->download(dv1);
    ovDV2->download(dv2);
    ovVSfp->download(sfp);
    ovAtomTreeSize->download(size);
    ovTreePointer->download(tree_pointer_t);
    ovProcessedFlag->download(processed);
    ovOKtoProcessFlag->download(oktoprocess);



    std::cout << "Tree:" << std::endl;
    for(int section = 0; section < num_sections; section++){
      std::cout << "Tree for sections: " << section << " " << " size= " << size[section] << std::endl;
      int pp = tree_pointer_t[section];
      int np = padded_tree_size[section];
      //self_volume += self_volumes[pp];
      std::cout << "slot level LastAtom parent ChStart ChCount SelfV V gamma Energy a x y z dedx dedy dedz sfp processed ok2process children_reported" << endl;
      for(int i = pp; i < pp + np ; i++){
	int maxprint = pp + 1024;
	if(i<maxprint){
	  std::cout << std::setprecision(4) << std::setw(6) << i << " "  << std::setw(7) << (int)level[i] << " " << std::setw(7) << (int)last_atom[i] << " " << std::setw(7) << (int)parent[i] << " "  << std::setw(7) << (int)children_start_index[i] << " " << std::setw(7) <<  (int)children_count[i] << " " << std::setw(15) << (float)self_volumes[i] << " " << std::setw(10) << (float)volumes[i]  << " " << std::setw(10) << (float)gammas[i] << " " << std::setw(10) << (float)energies[i] << " " << std::setw(10) << g[i].w << " " << std::setw(10) << g[i].x << " " << std::setw(10) << g[i].y << " " << std::setw(10) << g[i].z << " " << std::setw(10) << dv2[i].x << " " << std::setw(10) << dv2[i].y << " " << std::setw(10) << dv2[i].z << " " << std::setw(10) << sfp[i] << " " << processed[i] << " " << oktoprocess[i] << " " << children_reported[i] << std::endl;
	}
      }
    }
    //std::cout << "Volume (from self volumes):" << self_volume <<std::endl;
  }

  
  
  //------------------------------------------------------------------------------------------------------------
  //volume energy function 2, surface area cavity energy function
  //

  //seeds tree with "negative" gammas and reduced radi
  if(verbose_level > 2) cout << "Executing InitOverlapTreeKernel_1body_2 " << endl;
  cl.executeKernel(InitOverlapTreeKernel_1body_2, ov_work_group_size*num_compute_units, ov_work_group_size);

  if(verbose_level > 2) cout << "Executing ResetRescanOverlapTreeKernel" << endl;
  cl.executeKernel(ResetRescanOverlapTreeKernel, ov_work_group_size*num_compute_units, ov_work_group_size);
  
  if(verbose_level > 2) cout << "Executing InitRescanOverlapTreeKernel" << endl;
  cl.executeKernel(InitRescanOverlapTreeKernel, ov_work_group_size*num_compute_units, ov_work_group_size);
  
  if(verbose_level > 2) cout << "Executing RescanOverlapTreeKernel" << endl;
  cl.executeKernel(RescanOverlapTreeKernel, ov_work_group_size*num_compute_units, ov_work_group_size);
  
  if(verbose_level > 2) cout << "Executing resetSelfVolumesKernel" << endl;
  cl.executeKernel(resetSelfVolumesKernel, ov_work_group_size*num_compute_units, ov_work_group_size);
  
  if(verbose_level > 2) cout << "Executing computeSelfVolumesKernel" << endl;
  cl.executeKernel(computeSelfVolumesKernel, ov_work_group_size*num_compute_units, ov_work_group_size);

  //update energyBuffer with volume energy 2
  if(verbose_level > 2) cout << "Executing reduceSelfVolumesKernel_buffer" << endl;
  cl.executeKernel(reduceSelfVolumesKernel_buffer, ov_work_group_size*num_compute_units, ov_work_group_size);


  if(verbose){
    vector<int> atom_pointer(cl.getPaddedNumAtoms());
    vector<float> vol_energies(total_tree_size);
    ovAtomTreePointer->download(atom_pointer);
    ovVolEnergy->download(vol_energies);
    double energy = 0;
    for(int i=0;i<numParticles;i++){
      int slot = atom_pointer[i];
      energy += vol_energies[slot];
    }
    cout << "Volume Energy 2:" << energy << endl;
  }
  
  if(verbose_level > 4){
    float self_volume = 0.0;
    vector<cl_float> self_volumes(total_tree_size);
    vector<cl_float> volumes(total_tree_size);
    vector<cl_float> gammas(total_tree_size);
    vector<cl_int> last_atom(total_tree_size);
    vector<cl_int> level(total_tree_size);
    vector<cl_int> parent(total_tree_size);
    vector<cl_int> children_start_index(total_tree_size);
    vector<cl_int> children_count(total_tree_size);
    vector<cl_int> children_reported(total_tree_size);
    vector<mm_float4> g(total_tree_size);
    vector<mm_float4> dv2(total_tree_size);
    vector<cl_float> sfp(total_tree_size);
    vector<int> size(num_sections);
    vector<int> tree_pointer_t(num_sections);
    vector<cl_int> processed(total_tree_size);
    vector<cl_int> oktoprocess(total_tree_size);


    ovSelfVolume->download(self_volumes);
    ovVolume->download(volumes);
    ovLevel->download(level);
    ovLastAtom->download(last_atom);
    ovRootIndex->download(parent);
    ovChildrenStartIndex->download(children_start_index);
    ovChildrenCount->download(children_count);
    ovChildrenReported->download(children_reported);
    ovG->download(g);
    ovGamma1i->download(gammas);
    ovDV2->download(dv2);
    ovVSfp->download(sfp);
    ovAtomTreeSize->download(size);
    ovTreePointer->download(tree_pointer_t);
    ovProcessedFlag->download(processed);
    ovOKtoProcessFlag->download(oktoprocess);



    std::cout << "Tree:" << std::endl;
    for(int section = 0; section < num_sections; section++){
      std::cout << "Tree for sections: " << section << " " << " size= " << size[section] << std::endl;
      int pp = tree_pointer_t[section];
      int np = padded_tree_size[section];
      //self_volume += self_volumes[pp];
      std::cout << "slot level LastAtom parent ChStart ChCount SelfV V gamma a x y z dedx dedy dedz sfp processed ok2process children_reported" << endl;
      for(int i = pp; i < pp + np ; i++){
	int maxprint = pp + 1024;
	if(i<maxprint){
	  std::cout << std::setprecision(4) << std::setw(6) << i << " "  << std::setw(7) << (int)level[i] << " " << std::setw(7) << (int)last_atom[i] << " " << std::setw(7) << (int)parent[i] << " "  << std::setw(7) << (int)children_start_index[i] << " " << std::setw(7) <<  (int)children_count[i] << " " << std::setw(15) << (float)self_volumes[i] << " " << std::setw(10) << (float)volumes[i]  << " " << std::setw(10) << (float)gammas[i] << " " << std::setw(10) << g[i].w << " " << std::setw(10) << g[i].x << " " << std::setw(10) << g[i].y << " " << std::setw(10) << g[i].z << " " << std::setw(10) << dv2[i].x << " " << std::setw(10) << dv2[i].y << " " << std::setw(10) << dv2[i].z << " " << std::setw(10) << sfp[i] << " " << processed[i] << " " << oktoprocess[i] << " " << children_reported[i] << std::endl;
	}
      }
    }
    //std::cout << "Volume (from self volumes):" << self_volume <<std::endl;
  }
  
  if(false){
    vector<int> size(num_sections);
    vector<int> niter(num_sections);
    ovAtomTreeSize->download(size);
    cout << "Sizes: ";
    for(int section = 0; section < num_sections; section++){
      std::cout << size[section] << " ";
    }
    std::cout << endl;
    NIterations->download(niter);
    cout << "Niter: ";
    for(int section = 0; section < num_sections; section++){
      std::cout << niter[section] << " ";
    }
    std::cout << endl;

  }

  //temp arrays
  if(false){
    vector<cl_int> i_buffer(temp_buffer_size);
    vector<cl_int> atomj_buffer(temp_buffer_size);
    vector<cl_uint> tree_pos_buffer(temp_buffer_size);
    vector<cl_float> gvol_buffer(temp_buffer_size);
    int stride = ov_work_group_size*64;
    i_buffer_temp->download(i_buffer);
    atomj_buffer_temp->download(atomj_buffer);
    tree_pos_buffer_temp->download(tree_pos_buffer);
    gvol_buffer_temp->download(gvol_buffer);
    std::cout << "i_buffer   " << "atomj_buffer  " << "gvol   " << " fij  " <<  std::endl;
    for(int sect = 0; sect < num_sections; sect++){
      int buffer_offset = stride*sect;
      for(int j = 0; j < 640 ; j++){
	int i = j + buffer_offset;
	std::cout << i << " " << i_buffer[i] << " " << atomj_buffer[i] << " " << gvol_buffer[i] << " " << tree_pos_buffer[i] << std::endl;
      }
    }
  }

  //gammas
  if(false){
    float mol_volume = 0.0;
    vector<cl_float> gamma(num_sections);
    AtomicGamma->download(gamma);
    std::cout << "Gammas:" << std::endl;
    for(int iat = 0; iat < numParticles; iat++){
      std::cout << iat << " " << gamma[iat] << std::endl;
    }
  }



#ifdef NOTNOW
  //force/energy buffer
  if(false){
    if(useLong){
      // TO BE FIXED: OpenMM's energy buffer is the max number of threads, not the number of atoms
      vector<cl_float> energies(cl.getPaddedNumAtoms());

      vector<cl_long> energies_long(cl.getPaddedNumAtoms());
      cl.getEnergyBuffer().download(energies);
      std::cout << "OpenMM Energy Buffer:" << std::endl;
      for(int i = 0; i < cl.getPaddedNumAtoms(); i++){
	std::cout << i << " " << energies[i] << std::endl;
      }
      ovEnergyBuffer_long->download(energies_long);
      std::cout << "Long Energy Buffer:" << std::endl;
      float scale = 1/(float) 0x100000000;
      for(int i = 0; i < cl.getPaddedNumAtoms(); i++){
	std::cout << i << " " << scale*energies_long[i] << std::endl;
      }
    }else{
      float mol_volume = 0.0;
      vector<mm_float4> dv2(num_sections*cl.getPaddedNumAtoms());
      ovAtomBuffer->download(dv2);
      
      std::cout << "Atom Buffer:" << std::endl;
      for(int i = 0; i < num_sections*cl.getPaddedNumAtoms(); i++){
	if(i%cl.getPaddedNumAtoms()==0){
	  std::cout << "Tree: " << i/cl.getPaddedNumAtoms() << " =======================" << endl;
	}
	std::cout << i%cl.getPaddedNumAtoms() << " " << dv2[i].x << " " << dv2[i].y << " " << dv2[i].z << " " << dv2[i].w << std::endl;
      }
    }



    //std::cout << "Self Volumes:" << std::endl;
    //for(int iat = 0; iat < numParticles; iat++){
    //  std::cout << iat << " " << dv2[iat].w << std::endl;
    //  mol_volume += dv2[iat].w;
    //}
    //std::cout << "Volume (from self volumes):" << mol_volume <<std::endl;
  }
#endif

  if(verbose_level > 2) cout << "Done with GVolSA energy" << endl;

  return 0.0;
}

void OpenCLCalcAGBNPForceKernel::copyParametersToContext(ContextImpl& context, const AGBNPForce& force) {
    int numContexts = cl.getPlatformData().contexts.size();
    int startIndex = cl.getContextIndex()*force.getNumParticles()/numContexts;
    int endIndex = (cl.getContextIndex()+1)*force.getNumParticles()/numContexts;
    if (numParticles != endIndex-startIndex)
        throw OpenMMException("updateParametersInContext: The number of bonds has changed");
    if (numParticles == 0)
        return;
    
    // Record the per-bond parameters.
    
    //vector<mm_float2> paramVector(numBonds);
    //for (int i = 0; i < numBonds; i++) {
    //    int atom1, atom2;
    //   double length, k;
    //   force.getBondParameters(startIndex+i, atom1, atom2, length, k);
    //    paramVector[i] = mm_float2((cl_float) length, (cl_float) k);
    // }
    //params->upload(paramVector);
    
    // Mark that the current reordering may be invalid.
    
    cl.invalidateMolecules();
}
